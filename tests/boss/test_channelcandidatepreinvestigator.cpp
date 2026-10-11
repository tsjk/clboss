#undef NDEBUG
#include"Boss/Mod/ChannelCandidatePreinvestigator.hpp"
#include"Boss/Msg/PreinvestigateChannelCandidates.hpp"
#include"Boss/Msg/ProposeChannelCandidates.hpp"
#include"Boss/Msg/RequestConnect.hpp"
#include"Boss/Msg/ResponseConnect.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<assert.h>
#include<functional>
#include<string>
#include<utility>
#include<vector>

namespace {

/* A dummy Connector that DEFERS its responses.
 *
 * The real Connector performs the actual connection
 * asynchronously, so several preinvestigation Cases can be
 * parked in the module case-map before any ResponseConnect
 * arrives.  A connector that answers synchronously (as in
 * test_needsconnectsolicitor) lets every Case finish before
 * the next one registers and cannot reproduce the same-node
 * case collision.  */
class DeferredConnector {
private:
	S::Bus& bus;
	std::vector<std::string> pending;

public:
	size_t connects;

	explicit
	DeferredConnector(S::Bus& bus_) : bus(bus_), pending() {
		connects = 0;
		bus.subscribe<Boss::Msg::RequestConnect
			     >([this](Boss::Msg::RequestConnect const& req) {
			++connects;
			pending.push_back(req.node);
			return Ev::lift();
		});
	}
	DeferredConnector(DeferredConnector&&) =delete;

	/* Answer every pending connect, in arrival order.  New
	 * requests raised while flushing (a surviving Case moving
	 * on to its next candidate) stay pending for the next
	 * flush.  */
	Ev::Io<void> flush(bool success) {
		return Ev::lift().then([this, success]() {
			auto to_answer = std::move(pending);
			pending.clear();
			auto act = Ev::lift();
			for (auto& node : to_answer) {
				act += bus.raise(Boss::Msg::ResponseConnect{
					std::move(node), success
				});
			}
			return act;
		});
	}
};

class ProposalCollector {
public:
	std::vector<std::pair<std::string, std::string>> proposals;

	explicit
	ProposalCollector(S::Bus& bus) {
		bus.subscribe<Boss::Msg::ProposeChannelCandidates
			     >([this](Boss::Msg::ProposeChannelCandidates const& p) {
				proposals.emplace_back( std::string(p.proposal)
						      , std::string(p.patron)
						      );
				return Ev::lift();
		});
	}
	ProposalCollector(ProposalCollector&&) =delete;

	size_t
	count(std::string const& node, std::string const& patron) {
		auto n = size_t(0);
		for (auto const& p : proposals) {
			if (p.first == node && p.second == patron)
				++n;
		}
		return n;
	}
};

/* Valid 33-byte node ids (Ln::NodeId validates hex).  */
auto const HUB = std::string("02")
	+ "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	+ "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	;
auto const OTHER = std::string("02")
	+ "cccccccccccccccccccccccccccccccc"
	+ "cccccccccccccccccccccccccccccccc"
	;
auto const OTHER2 = std::string("02")
	+ "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	+ "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	;
auto const PATRON1 = std::string("02")
	+ "d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1"
	+ "d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1"
	;
auto const PATRON2 = std::string("02")
	+ "d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2"
	+ "d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2"
	;
auto const PATRON3 = std::string("02")
	+ "d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3"
	+ "d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3d3"
	;
auto const PATRON4 = std::string("02")
	+ "d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4"
	+ "d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4d4"
	;
auto const PATRON5 = std::string("02")
	+ "d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5"
	+ "d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5d5"
	;
auto const PATRON6 = std::string("02")
	+ "d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6"
	+ "d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6"
	;

}

int main() {
	S::Bus bus;
	DeferredConnector connector(bus);
	ProposalCollector collector(bus);
	Boss::Mod::ChannelCandidatePreinvestigator preinv(bus);

	auto preinvestigate = [&bus]( std::vector<Boss::Msg::ProposeChannelCandidates> cs
				   , std::size_t max_candidates
				   ) {
		return bus.raise(Boss::Msg::PreinvestigateChannelCandidates{
			std::move(cs), max_candidates
		});
	};

	auto test = Ev::lift().then([&]() {
		/* Two finders concurrently preinvestigate batches
		 * that share the same proposal node (several leaf
		 * patrons whose only peer is the popular hub).  */
		return preinvestigate(
			{ Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(HUB), Ln::NodeId(PATRON1)
			} },
			1
		);
	}).then([&]() {
		return preinvestigate(
			{ Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(HUB), Ln::NodeId(PATRON2)
			} },
			1
		);
	}).then([&]() {
		/* Every Case queued on a node asks the same
		 * question of it: exactly ONE connect is raised
		 * for the shared node, not one per Case.  */
		assert(connector.connects == 1);
		/* And nothing has completed: no proposal can
		 * surface before any ResponseConnect.  */
		assert(collector.proposals.empty());

		/* The real Connector answers asynchronously, with
		 * a single ResponseConnect for the single
		 * RequestConnect.  */
		return connector.flush(true);
	}).then([&]() {
		/* The single response's outcome is handed to BOTH
		 * waiting Cases: each patron's proposal surfaces
		 * with its own attribution.  */
		assert(collector.count(HUB, PATRON1) == 1);
		assert(collector.count(HUB, PATRON2) == 1);
		assert(collector.proposals.size() == 2);

		/* Regression guard: a later, non-colliding batch
		 * still surfaces normally.  */
		return preinvestigate(
			{ Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(OTHER), Ln::NodeId(PATRON3)
			} },
			1
		);
	}).then([&]() {
		assert(connector.connects == 2);
		return connector.flush(true);
	}).then([&]() {
		assert(collector.count(OTHER, PATRON3) == 1);
		assert(collector.proposals.size() == 3);

		/* Collision on the failure path, with the
		 * surviving Case owing more candidates: PATRON4
		 * has a second candidate (OTHER2) that may only
		 * be proposed if its Case survives the failed
		 * connect to the shared node.  */
		return preinvestigate(
			{ Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(HUB), Ln::NodeId(PATRON4)
			}
			, Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(OTHER2), Ln::NodeId(PATRON4)
			} },
			2
		);
	}).then([&]() {
		return preinvestigate(
			{ Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(HUB), Ln::NodeId(PATRON5)
			} },
			1
		);
	}).then([&]() {
		/* Again one connect for the shared node: the
		 * duplicate-connect removal holds on the failure
		 * path too.  */
		assert(connector.connects == 3);
		/* The single connect to the shared node fails.  */
		return connector.flush(false);
	}).then([&]() {
		/* The failure is fanned out: nothing proposed from
		 * the failed connects...  */
		assert(collector.proposals.size() == 3);
		/* ...but PATRON4's Case lives on and has moved to
		 * its NEXT candidate, raising a fresh connect for
		 * it; PATRON5's Case, out of candidates, ends.  */
		assert(connector.connects == 4);
		return connector.flush(true);
	}).then([&]() {
		assert(collector.count(OTHER2, PATRON4) == 1);
		assert(collector.proposals.size() == 4);

		/* Self-re-registration: a Case whose batch
		 * contains the same node again re-registers it
		 * from within the fan-out of its own response;
		 * the entry was erased before execution, so the
		 * re-registration lands in a fresh entry and
		 * triggers a fresh connect rather than being
		 * consumed by the in-flight response.  */
		return preinvestigate(
			{ Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(HUB), Ln::NodeId(PATRON6)
			}
			, Boss::Msg::ProposeChannelCandidates{
				Ln::NodeId(HUB), Ln::NodeId(PATRON6)
			} },
			2
		);
	}).then([&]() {
		assert(connector.connects == 5);
		return connector.flush(true);
	}).then([&]() {
		/* The first HUB proposal surfaced and the Case
		 * re-registered HUB for its second candidate.  */
		assert(collector.count(HUB, PATRON6) == 1);
		assert(connector.connects == 6);
		return connector.flush(true);
	}).then([&]() {
		assert(collector.count(HUB, PATRON6) == 2);
		assert(collector.proposals.size() == 6);

		return Ev::lift(0);
	});

	return Ev::start(test);
}

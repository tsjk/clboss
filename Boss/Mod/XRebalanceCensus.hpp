#ifndef BOSS_MOD_XREBALANCECENSUS_HPP
#define BOSS_MOD_XREBALANCECENSUS_HPP

#include<cstddef>
#include<string>

namespace Jsmn { class Object; }

namespace Boss { namespace Mod {

/** struct Boss::Mod::XRebalanceCensus
 *
 * @brief Part counts and the chokepoint of one `xrebalance`
 * transfer, digested from the plugin's response for the
 * XRebalancer summary log line.
 *
 * Plugin v0.4.4 and later return a `summary` object (part
 * counts, `pending_msat`, and `closest_miss` when nothing
 * completed or is pending) and include the per-part arrays
 * only on request; the summary is read first.  Without it
 * the census walks the parts arrays: a single-shot response
 * carries a top-level `parts`, a multi-round one nests one
 * per entry of `rounds`.
 */
struct XRebalanceCensus {
	std::size_t parts_total{0};
	std::size_t parts_complete{0};
	std::size_t parts_pending{0};
	std::size_t parts_failed{0};
	/* msat still in flight, or -1 when the response does not
	 * say.  */
	double pending_msat{-1.0};
	/* "; closest failure: ..." for the failed part that got
	 * nearest to delivery (fewest hops short), with a
	 * "[closest of N]" tail when more than one failed; empty
	 * when a part completed or is pending, since that part is
	 * the frontier.  */
	std::string reason;

	static XRebalanceCensus from_response(Jsmn::Object const& res);
};

}}

#endif /* !defined(BOSS_MOD_XREBALANCECENSUS_HPP) */

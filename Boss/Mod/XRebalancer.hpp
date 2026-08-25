#ifndef BOSS_MOD_XREBALANCER_HPP
#define BOSS_MOD_XREBALANCER_HPP

#include"Ev/now.hpp"
#include<functional>
#include<memory>

namespace Boss { namespace Mod { class Waiter; }}
namespace S { class Bus; }

namespace Boss { namespace Mod {

/** class Boss::Mod::XRebalancer
 *
 * @brief The xrebalance rebalancer driver: a periodic, Poisson-paced
 * loop that decides WHEN to run a circular-askrene rebalance cycle,
 * plans it, and executes it through the external xrebalance plugin.
 *
 * Cadence is a memoryless Poisson process whose average rate is the
 * `clboss-xrebalance-per-hour` option (0 = paused), tunable at runtime.
 * The loop self-gates on the rebalance mode and only runs a cycle when
 * the active mode is `xrebalance`; in other modes it idles (skips the
 * cycle body) so flipping the mode at runtime starts firing without a
 * restart.
 *
 * Reports the plugin's health under the `xrebalancer` key of
 * `clboss-status`, and warns when the plugin is not loaded.  The
 * `get_now` argument is the clock for those timestamps and for the
 * hourly repeat of that warning; tests pass a fake.
 */
class XRebalancer {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	XRebalancer() =delete;
	XRebalancer(XRebalancer const&) =delete;

	XRebalancer(XRebalancer&&);
	~XRebalancer();

	explicit
	XRebalancer( S::Bus& bus
		   , Boss::Mod::Waiter& waiter
		   , std::function<double()> get_now = &Ev::now
		   );
};

}}

#endif /* !defined(BOSS_MOD_XREBALANCER_HPP) */

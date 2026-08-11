#ifndef BOSS_MOD_FUNDSMOVER_CLAIMER_HPP
#define BOSS_MOD_FUNDSMOVER_CLAIMER_HPP

#include"Ln/Amount.hpp"
#include"Ln/Preimage.hpp"
#include"Secp256k1/Random.hpp"
#include"Sha256/Hash.hpp"
#include<unordered_map>

namespace S { class Bus; }

namespace Boss { namespace Mod { namespace FundsMover {

/** class Boss::Mod::FundsMover::Claimer
 *
 * @brief claims incoming funds at the destination.
 */
class Claimer {
private:
	S::Bus& bus;

	Secp256k1::Random rand;

	struct Entry {
		double timeout;
		Ln::Preimage preimage;
		Ln::Preimage payment_secret;
		Ln::Amount expected_amount;
	};
	std::unordered_map<Sha256::Hash, Entry> entries;

	void start();

public:
	Claimer() =delete;
	Claimer(Claimer&&) =delete;
	Claimer(Claimer const&) =delete;

	explicit
	Claimer(S::Bus& bus_) : bus(bus_) { start(); }

	/* Generate a new preimage and payment secret for a new
	 * Attempter, which intends to receive exactly
	 * `expected_amount` at the destination.  */
	std::pair<Ln::Preimage, Ln::Preimage> generate(Ln::Amount expected_amount);
};

}}}

#endif /* !defined(BOSS_MOD_FUNDSMOVER_CLAIMER_HPP) */

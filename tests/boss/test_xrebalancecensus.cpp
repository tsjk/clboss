#undef NDEBUG
#include"Boss/Mod/XRebalanceCensus.hpp"
#include"Jsmn/Object.hpp"
#include<assert.h>
#include<string>

/* The XRebalancer summary line's part counts, pending note, and
 * chokepoint come from XRebalanceCensus.  Plugin v0.4.4+ returns
 * a summary object and omits the per-part arrays unless asked;
 * older plugins and single-shot responses carry the arrays.  */

namespace {

/* Plugin v0.4.5 multi-round response, nothing delivered: the
 * summary carries the counts and the closest miss, and there is
 * no rounds array.  */
auto const summary_failed = R"JSON(
{
  "status": "executed",
  "label": "7b85ba0c",
  "amount_msat": 476940000,
  "rounds_run": 3,
  "stop_reason": "no further routes: no usable route from the sources to the destinations at this amount and budget, nor at smaller amounts (getroutes 205)",
  "delivered_msat": 0,
  "fee_msat": 0,
  "fee_ppm": 0,
  "pending_msat": 0,
  "summary": {
    "rounds": 3,
    "parts": 12,
    "parts_complete": 0,
    "parts_failed": 12,
    "parts_pending": 0,
    "delivered_msat": 0,
    "fee_msat": 0,
    "fee_ppm": 0,
    "pending_msat": 0,
    "closest_miss": {
      "hops_short": 1,
      "planned_msat": 393516,
      "erring_scidd": "949996x1528x0/0",
      "failcode": 4103,
      "failcode_name": "TEMPORARY_CHANNEL_FAILURE"
    }
  }
}
)JSON";

/* v0.4.5 response with delivery and one straggler: no
 * closest_miss, pending_msat says how much is still settling.  */
auto const summary_partial = R"JSON(
{
  "status": "executed",
  "label": "fe4fd15e",
  "amount_msat": 2000000,
  "rounds_run": 4,
  "stop_reason": "budget exhausted",
  "delivered_msat": 1177428,
  "fee_msat": 268,
  "fee_ppm": 228,
  "pending_msat": 598971,
  "summary": {
    "rounds": 4,
    "parts": 3,
    "parts_complete": 2,
    "parts_failed": 0,
    "parts_pending": 1,
    "delivered_msat": 1177428,
    "fee_msat": 268,
    "fee_ppm": 228,
    "pending_msat": 598971
  }
}
)JSON";

/* Pre-0.4.4 multi-round response: per-round parts arrays, no
 * summary.  The part with the fewest hops short is the
 * chokepoint; a part without hops_short never wins.  */
auto const legacy_rounds = R"JSON(
{
  "status": "executed",
  "label": "0b01d715",
  "rounds_run": 2,
  "delivered_msat": 0,
  "fee_msat": 0,
  "pending_msat": 0,
  "rounds": [
    { "delivered_msat": 0,
      "parts": [
        { "status": "failed", "planned_msat": 500000, "hops_short": 3,
          "erring_scidd": "900000x1x0/1", "failcode": 4103 },
        { "status": "failed", "planned_msat": 250000, "hops_short": 2,
          "erring_scidd": "910000x2x1/0", "failcode": 4108 }
      ]
    },
    { "delivered_msat": 0,
      "parts": [
        { "status": "failed", "planned_msat": 125000, "hops_short": null,
          "erring_scidd": null, "failcode": null }
      ]
    }
  ]
}
)JSON";

/* Single-shot response (rounds_max 1): a top-level parts array,
 * no summary, no rounds.  A completed or pending part is the
 * frontier, so no closest failure is reported.  */
auto const single_shot = R"JSON(
{
  "status": "executed",
  "label": "416a8044",
  "delivered_msat": 1644635,
  "fee_msat": 358,
  "pending_msat": 250000,
  "parts": [
    { "status": "complete", "planned_msat": 1644635, "hops_short": null },
    { "status": "pending", "planned_msat": 250000, "hops_short": null },
    { "status": "failed", "planned_msat": 100000, "hops_short": 1,
      "erring_scidd": "920000x3x0/0", "failcode": 4103 }
  ]
}
)JSON";

/* verbose:true: summary and rounds both present.  The summary
 * is the census; the arrays are not counted a second time.  */
auto const summary_and_rounds = R"JSON(
{
  "status": "executed",
  "delivered_msat": 0,
  "fee_msat": 0,
  "pending_msat": 0,
  "summary": {
    "rounds": 1,
    "parts": 2,
    "parts_complete": 0,
    "parts_failed": 2,
    "parts_pending": 0,
    "pending_msat": 0,
    "closest_miss": { "hops_short": 2, "planned_msat": 100,
                      "erring_scidd": "930000x4x0/1", "failcode": 4103,
                      "failcode_name": "TEMPORARY_CHANNEL_FAILURE" }
  },
  "rounds": [
    { "parts": [
        { "status": "failed", "planned_msat": 100, "hops_short": 2,
          "erring_scidd": "930000x4x0/1", "failcode": 4103 },
        { "status": "failed", "planned_msat": 50, "hops_short": 4,
          "erring_scidd": "940000x5x0/1", "failcode": 4103 }
      ]
    }
  ]
}
)JSON";

}

int main() {
	using Boss::Mod::XRebalanceCensus;

	{
		auto c = XRebalanceCensus::from_response(
			Jsmn::Object::parse_json(summary_failed)
		);
		assert(c.parts_total == 12);
		assert(c.parts_complete == 0);
		assert(c.parts_pending == 0);
		assert(c.parts_failed == 12);
		assert(c.pending_msat == 0.0);
		assert(c.reason == std::string(
			"; closest failure: 1 hops short at 949996x1528x0/0"
			" (failcode 0x1007 TEMPORARY_CHANNEL_FAILURE)"
			" [closest of 12]"
		));
	}

	{
		auto c = XRebalanceCensus::from_response(
			Jsmn::Object::parse_json(summary_partial)
		);
		assert(c.parts_total == 3);
		assert(c.parts_complete == 2);
		assert(c.parts_pending == 1);
		assert(c.parts_failed == 0);
		assert(c.pending_msat == 598971.0);
		assert(c.reason.empty());
	}

	{
		auto c = XRebalanceCensus::from_response(
			Jsmn::Object::parse_json(legacy_rounds)
		);
		assert(c.parts_total == 3);
		assert(c.parts_complete == 0);
		assert(c.parts_pending == 0);
		assert(c.parts_failed == 3);
		assert(c.pending_msat == 0.0);
		assert(c.reason == std::string(
			"; closest failure: 2 hops short at 910000x2x1/0"
			" (failcode 0x100c) [closest of 3]"
		));
	}

	{
		auto c = XRebalanceCensus::from_response(
			Jsmn::Object::parse_json(single_shot)
		);
		assert(c.parts_total == 3);
		assert(c.parts_complete == 1);
		assert(c.parts_pending == 1);
		assert(c.parts_failed == 1);
		assert(c.pending_msat == 250000.0);
		assert(c.reason.empty());
	}

	{
		auto c = XRebalanceCensus::from_response(
			Jsmn::Object::parse_json(summary_and_rounds)
		);
		assert(c.parts_total == 2);
		assert(c.parts_failed == 2);
		assert(c.reason == std::string(
			"; closest failure: 2 hops short at 930000x4x0/1"
			" (failcode 0x1007 TEMPORARY_CHANNEL_FAILURE)"
			" [closest of 2]"
		));
	}

	{
		/* No parts information at all.  */
		auto c = XRebalanceCensus::from_response(
			Jsmn::Object::parse_json("{}")
		);
		assert(c.parts_total == 0);
		assert(c.parts_pending == 0);
		assert(c.pending_msat == -1.0);
		assert(c.reason.empty());
		/* Not an object at all.  */
		auto n = XRebalanceCensus::from_response(
			Jsmn::Object::parse_json("[]")
		);
		assert(n.parts_total == 0);
		assert(n.pending_msat == -1.0);
		assert(n.reason.empty());
	}

	return 0;
}

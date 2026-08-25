#include"Boss/Mod/XRebalanceCensus.hpp"
#include"Jsmn/Object.hpp"
#include<cmath>
#include<limits>
#include<sstream>

namespace {

double num_or(Jsmn::Object const& o, char const* k, double dflt) {
	if (o.is_object() && o.has(k)) {
		auto v = o[k];
		if (v.is_number())
			return double(v);
	}
	return dflt;
}
std::size_t count_or_zero(Jsmn::Object const& o, char const* k) {
	auto v = num_or(o, k, 0.0);
	return v > 0.0 ? std::size_t(std::llround(v)) : std::size_t(0);
}

/* "; closest failure: N hops short at <scidd> (failcode 0x..
 * NAME)", each piece present only when the object carries it.
 * Shared by the summary's closest_miss and the per-part walk,
 * whose objects use the same field names.  */
std::string describe_miss(Jsmn::Object const& p) {
	auto os = std::ostringstream();
	os << "; closest failure:";
	if (p.has("hops_short") && p["hops_short"].is_number())
		os << " " << std::llround(double(p["hops_short"]))
		   << " hops short";
	if (p.has("erring_scidd") && p["erring_scidd"].is_string())
		os << " at " << std::string(p["erring_scidd"]);
	if (p.has("failcode") && p["failcode"].is_number()) {
		os << " (failcode 0x" << std::hex
		   << std::llround(double(p["failcode"])) << std::dec;
		if (p.has("failcode_name")
		 && p["failcode_name"].is_string())
			os << " " << std::string(p["failcode_name"]);
		os << ")";
	}
	return os.str();
}

}

namespace Boss { namespace Mod {

XRebalanceCensus
XRebalanceCensus::from_response(Jsmn::Object const& res) {
	auto c = XRebalanceCensus();
	if (!res.is_object())
		return c;

	c.pending_msat = num_or(res, "pending_msat", -1.0);

	if (res.has("summary") && res["summary"].is_object()) {
		auto s = res["summary"];
		c.parts_total = count_or_zero(s, "parts");
		c.parts_complete = count_or_zero(s, "parts_complete");
		c.parts_pending = count_or_zero(s, "parts_pending");
		c.parts_failed = count_or_zero(s, "parts_failed");
		c.pending_msat = num_or(s, "pending_msat", c.pending_msat);
		/* The plugin emits closest_miss only when nothing
		 * completed or is pending, the same rule the walk
		 * below applies.  */
		if (s.has("closest_miss") && s["closest_miss"].is_object())
			c.reason = describe_miss(s["closest_miss"]);
	} else {
		/* Among failed parts, the one that got closest to
		 * delivery (smallest hops_short) is the informative
		 * frontier.  Kept only when nothing completed or is
		 * pending: a completed part IS the frontier, and
		 * outranks any near-miss.  */
		auto best_short = std::numeric_limits<double>::max();
		auto walk = [&](Jsmn::Object parts) {
			c.parts_total += parts.size();
			for (auto i = std::size_t(0); i < parts.size(); ++i) {
				auto p = parts[i];
				if (!p.is_object() || !p.has("status")
				 || !p["status"].is_string())
					continue;
				auto st = std::string(p["status"]);
				if (st == "complete") {
					++c.parts_complete;
					continue;
				}
				if (st == "pending") {
					++c.parts_pending;
					continue;
				}
				++c.parts_failed;
				auto hs = num_or( p, "hops_short"
						, std::numeric_limits<double>::max()
						);
				if (!c.reason.empty() && hs >= best_short)
					continue;
				best_short = hs;
				c.reason = describe_miss(p);
			}
		};
		if (res.has("parts") && res["parts"].is_array())
			walk(res["parts"]);
		if (res.has("rounds") && res["rounds"].is_array()) {
			auto rounds = res["rounds"];
			for (auto i = std::size_t(0); i < rounds.size(); ++i) {
				auto r = rounds[i];
				if (r.is_object() && r.has("parts")
				 && r["parts"].is_array())
					walk(r["parts"]);
			}
		}
		if (c.parts_complete > 0 || c.parts_pending > 0)
			c.reason.clear();
	}

	if (!c.reason.empty() && c.parts_failed > 1)
		c.reason += " [closest of "
			  + std::to_string(c.parts_failed) + "]";
	return c;
}

}}

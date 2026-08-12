#undef NDEBUG
#include"Boltz/Detail/ClaimTxHandler.hpp"
#include"Boltz/EnvIF.hpp"
#include"Bitcoin/Tx.hpp"
#include"Bitcoin/TxId.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Preimage.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Secp256k1/Signature.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sha256/Hash.hpp"
#include"Sha256/fun.hpp"
#include"Sqlite3.hpp"
#include"Util/Str.hpp"
#include<assert.h>
#include<cstdint>
#include<memory>
#include<string>
#include<vector>

namespace {

/* Mock environment: fixed feerate, broadcast always succeeds
 * and records that it was called.
 */
class MockEnv : public Boltz::EnvIF {
private:
	bool& broadcast_called;

public:
	explicit
	MockEnv(bool& broadcast_called_)
		: broadcast_called(broadcast_called_) { }

	Ev::Io<std::uint32_t> get_feerate() override {
		return Ev::lift(std::uint32_t(1000));
	}
	Ev::Io<bool> broadcast_tx(Bitcoin::Tx) override {
		broadcast_called = true;
		return Ev::lift(true);
	}
	Ev::Io<void> logd(std::string) override { return Ev::lift(); }
	Ev::Io<void> loge(std::string) override { return Ev::lift(); }
};

/* Mock signer backed by a fixed privkey.  Nothing in the claim
 * path verifies the signature, so signing with an arbitrary key
 * is fine.
 */
class MockSigner : public Secp256k1::SignerIF {
private:
	Secp256k1::PrivKey privkey;

public:
	MockSigner()
		: privkey(Secp256k1::PrivKey(std::string(
			"0101010101010101010101010101010101010101010101010101010101010101"
		  )))
		{ }

	Secp256k1::PubKey get_pubkey_tweak(Secp256k1::PrivKey const&) override {
		return Secp256k1::PubKey(privkey);
	}
	Secp256k1::Signature get_signature_tweak( Secp256k1::PrivKey const&
						, Sha256::Hash const& m
						) override {
		return Secp256k1::Signature::create(privkey, m);
	}
	Sha256::Hash get_privkey_salted_hash(std::uint8_t salt[32]) override {
		return Sha256::fun(salt, 32);
	}
};

/* Table schema, copied from Boltz/ServiceFactory.cpp.  */
char const schema[] = R"QRY(
CREATE TABLE "BoltzServiceFactory_rsub"
     ( id INTEGER PRIMARY KEY
     , apiAccess TEXT NOT NULL
     , tweak TEXT NOT NULL
     , preimage TEXT NOT NULL
     , destinationAddress TEXT NOT NULL
     , swapId TEXT NOT NULL
     , redeemScript TEXT NOT NULL
     , timeoutBlockheight INTEGER NOT NULL
     , onchainAmount INTEGER NOT NULL
     , lockedUp INTEGER NOT NULL
     , lockupTxid TEXT NULL
     , lockupOut INTEGER NULL
     , lockupConfirmedHeight INTEGER NULL
     , lockupClaimFees INTEGER NULL
     , comment TEXT
     );
)QRY";

}

int main() {
	auto db = Sqlite3::Db(":memory:");

	auto const api = std::string("testapi");
	/* The claim path only hashes the redeem script and copies
	 * it into the claim witness; its content does not matter.
	 */
	auto const redeemScript = std::string("51");
	auto const tweak = std::string(
		"0202020202020202020202020202020202020202020202020202020202020202"
	);
	auto const preimage = std::string(
		"0303030303030303030303030303030303030303030303030303030303030303"
	);
	auto const addr = std::string(
		"bc1qg430dgu75qvphu8nn3kvcp3yg2km2tavpc8lqs"
	);
	auto const timeout = std::uint32_t(1000);
	auto const amount = std::uint64_t(100000);
	auto const blockheight = std::uint32_t(100);

	/* Lockup tx paying P2WSH of the redeem script.  */
	auto script_bytes = Util::Str::hexread(redeemScript);
	auto script_hash = Sha256::fun(&script_bytes[0], script_bytes.size());
	auto scriptPubKey = std::vector<std::uint8_t>(34);
	scriptPubKey[0] = 0x00;
	scriptPubKey[1] = 0x20;
	script_hash.to_buffer(&scriptPubKey[2]);

	auto lockup_tx = Bitcoin::Tx();
	lockup_tx.inputs.resize(1);
	lockup_tx.inputs[0].prevTxid = Bitcoin::TxId(std::string(
		"0404040404040404040404040404040404040404040404040404040404040404"
	));
	lockup_tx.inputs[0].prevOut = 0;
	lockup_tx.outputs.resize(1);
	lockup_tx.outputs[0].scriptPubKey = scriptPubKey;
	lockup_tx.outputs[0].amount = Ln::Amount::sat(amount);

	auto broadcast_called = bool(false);
	auto env = MockEnv(broadcast_called);
	auto signer = MockSigner();

	auto insert_swap = [&](std::string swapId) {
		return db.transact().then([&, swapId](Sqlite3::Tx tx) {
			tx.query(R"QRY(
			INSERT INTO "BoltzServiceFactory_rsub"
			     ( apiAccess
			     , tweak
			     , preimage
			     , destinationAddress
			     , swapId
			     , redeemScript
			     , timeoutBlockheight
			     , onchainAmount
			     , lockedUp
			     , comment
			     )
			VALUES
			     ( :apiAccess
			     , :tweak
			     , :preimage
			     , :destinationAddress
			     , :swapId
			     , :redeemScript
			     , :timeoutBlockheight
			     , :onchainAmount
			     , 0
			     , ''
			     );
			)QRY")
				.bind(":apiAccess", api)
				.bind(":tweak", tweak)
				.bind(":preimage", preimage)
				.bind(":destinationAddress", addr)
				.bind(":swapId", swapId)
				.bind(":redeemScript", redeemScript)
				.bind(":timeoutBlockheight", timeout)
				.bind(":onchainAmount", amount)
				.execute();
			tx.commit();
			return Ev::lift();
		});
	};

	auto code = Ev::lift().then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query_execute(schema);
		tx.commit();
		/* Two swaps in flight at once.  */
		return insert_swap("swapA");
	}).then([&]() {
		return insert_swap("swapB");
	}).then([&]() {
		/* Claim the first swap.  */
		auto handler = Boltz::Detail::ClaimTxHandler::create(
			signer, db, env, api, "swapA", blockheight, lockup_tx
		);
		return handler->run();
	}).then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		/* The handler must have reached the broadcast,
		 * otherwise the assertions below are vacuous.
		 */
		assert(broadcast_called);

		/* Sanity: the claimed swap was stamped.  */
		auto stamped = tx.query(R"QRY(
		SELECT lockedUp, lockupTxid
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapA'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		auto found = false;
		for (auto& r : stamped) {
			found = true;
			assert(r.get<bool>(0));
			assert(!r.get<std::string>(1).empty());
		}
		assert(found);

		/* Regression: the sibling swap must be untouched.  */
		auto sibling = tx.query(R"QRY(
		SELECT lockedUp
		  FROM "BoltzServiceFactory_rsub"
		 WHERE apiAccess = :apiAccess
		   AND swapId = 'swapB'
		     ;
		)QRY")
			.bind(":apiAccess", api)
			.execute();
		found = false;
		for (auto& r : sibling) {
			found = true;
			assert(!r.get<bool>(0));
		}
		assert(found);

		/* Exactly one row may be stamped overall.  */
		auto count = tx.query(R"QRY(
		SELECT COUNT(*) FROM "BoltzServiceFactory_rsub"
		 WHERE lockedUp <> 0
		    OR lockupTxid IS NOT NULL
		     ;
		)QRY").execute();
		found = false;
		for (auto& r : count) {
			found = true;
			assert(r.get<int>(0) == 1);
		}
		assert(found);

		tx.commit();
		return Ev::lift(0);
	});

	return Ev::start(code);
}

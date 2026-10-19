// Safe Input Buffer
//
// When you click, the mod simulates a few ticks ahead with that click applied.
// If you would die, the click is held back and re-tested every frame until it
// is safe (or until the max delay runs out, then it fires normally).
//
// NOTE: written without being able to run it in-game.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace ib {

	struct Pending {
		bool active = false;        // a click is being held back
		bool releaseQueued = false; // player let go while we were holding the click
		bool fired = false;         // click was sent, release still has to follow
		int frames = 0;             // frames waited so far
	};

	inline bool simulating = false; // true while the lookahead is running
	inline bool simDied = false;    // set when the lookahead player dies
	inline Pending pending[2];      // [0] = player 1, [1] = player 2

	inline bool enabled() {
		return Mod::get()->getSettingValue<bool>("enabled");
	}

	inline void clearAll() {
		pending[0] = {};
		pending[1] = {};
	}

	// Send input directly, without going through the input queue hook.
	inline void sendButton(GJBaseGameLayer* layer, bool down, bool p1) {
		layer->handleButton(down, 1, p1); // VERIFY: handleButton(bool down, int button, bool isPlayer1)
	}

	// Would pressing jump right now lead to death within the lookahead window?
	// Runs between frames (from the touch/key event), never inside update().
	inline bool wouldDie(PlayLayer* pl, bool p1) {
		int lookahead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-ticks"));

		simulating = true;
		simDied = false;

		// Snapshot the whole level state, the same way practice mode does.
		CheckpointObject* cp = pl->createCheckpoint();
		if (!cp) {
			simulating = false;
			return false;
		}
		cp->retain();

		// Apply the click and step the game forward.
		sendButton(pl, true, p1);
		for (int i = 0; i < lookahead && !simDied; i++) {
			pl->update(1.f / 240.f);
		}

		// Roll everything back and drop the simulated button state.
		pl->loadFromCheckpoint(cp);
		sendButton(pl, false, p1);
		cp->release();

		bool died = simDied;
		simulating = false;
		simDied = false;
		return died;
	}

	// Called once per rendered frame, after the real update.
	inline void tick(PlayLayer* pl) {
		int maxFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-delay-frames"));

		for (int i = 0; i < 2; i++) {
			auto& p = pending[i];
			if (!p.active) continue;
			bool p1 = (i == 0);

			// The click already went out; now send the release the player did meanwhile.
			if (p.fired) {
				sendButton(pl, false, p1);
				p = {};
				continue;
			}

			bool timedOut = ++p.frames > maxFrames;
			if (timedOut || !wouldDie(pl, p1)) {
				sendButton(pl, true, p1);
				if (p.releaseQueued) {
					p.fired = true; // release goes out next frame
				} else {
					p = {};         // player is still holding, real release passes through
				}
			}
		}
	}
}

class $modify(IBBaseLayer, GJBaseGameLayer) {
	void queueButton(int button, bool push, bool isPlayer2, double timestamp) {
		bool isPlayer1 = !isPlayer2;
		auto pl = PlayLayer::get();

		bool passThrough =
			ib::simulating || !ib::enabled() || !pl ||
			static_cast<GJBaseGameLayer*>(pl) != this || button != 1;

		if (passThrough) {
			GJBaseGameLayer::queueButton(button, push, isPlayer2, timestamp);
			return;
		}

		auto& p = ib::pending[isPlayer1 ? 0 : 1];

		if (push) {
			if (p.active) return; // already waiting on a click

			if (!ib::wouldDie(pl, isPlayer1)) {
				GJBaseGameLayer::queueButton(button, push, isPlayer2, timestamp);
				return;
			}

			// Unsafe: hold the click back, tick() will retry it every frame.
			p = {};
			p.active = true;
			return;
		}

		// Release while a click is still being held back: remember it for later.
		if (p.active) {
			p.releaseQueued = true;
			return;
		}

		GJBaseGameLayer::queueButton(button, push, isPlayer2, timestamp);
	}
};

class $modify(IBPlayLayer, PlayLayer) {
	void update(float dt) {
		PlayLayer::update(dt);
		if (ib::simulating || !ib::enabled()) return;
		ib::tick(this);
	}

	// During the lookahead, a death only sets a flag instead of really killing you.
	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		if (ib::simulating) {
			ib::simDied = true;
			return;
		}
		PlayLayer::destroyPlayer(player, obj);
	}

	void resetLevel() {
		ib::clearAll();
		PlayLayer::resetLevel();
	}

	void onQuit() {
		ib::clearAll();
		PlayLayer::onQuit();
	}
};

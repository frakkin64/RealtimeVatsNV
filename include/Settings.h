#pragma once

namespace Plugin
{
	namespace Settings
	{
		inline REX::TOML::I32 LogLevel{ "General"sv, "iLogLevel"sv, static_cast<std::int32_t>(REX::ELogLevel::Info) };

		inline REX::TOML::Bool Enabled{ "RealtimeVats"sv, "bEnabled"sv, true };

		// DirectInput scan codes (DIK_*): X locks on / releases, C cycles the target; 0 disables a key
		inline REX::TOML::I32 KeyLock{ "RealtimeVats"sv, "iKeyLock"sv, 0x2D };
		inline REX::TOML::I32 KeyNext{ "RealtimeVats"sv, "iKeyNext"sv, 0x2E };
		inline REX::TOML::I32 KeyPrevious{ "RealtimeVats"sv, "iKeyPrevious"sv, 0 };

		// target acquisition (game units / degrees)
		inline REX::TOML::F32 MaxRange{ "RealtimeVats"sv, "fMaxRange"sv, 2500.0f };
		inline REX::TOML::F32 MaxAngle{ "RealtimeVats"sv, "fMaxAngle"sv, 45.0f };

		// lowest hit chance (0..1) at which a target can be locked, cycled to or chained to
		inline REX::TOML::F32 MinChanceToLock{ "RealtimeVats"sv, "fMinChanceToLock"sv, 0.10f };

		// BGSBodyPart::LIMB_ENUM index aimed at (0 torso, 1 head, 3 left arm, 5 right arm, 7 left leg, 10 right leg)
		inline REX::TOML::I32 Limb{ "RealtimeVats"sv, "iLimb"sv, 0 };

		// keys that cycle the aimed body part among torso, head, arms and legs (Z; the previous-limb key is off by default)
		inline REX::TOML::I32 KeyLimbPrevious{ "RealtimeVats"sv, "iKeyLimbPrevious"sv, 0 };
		inline REX::TOML::I32 KeyLimbNext{ "RealtimeVats"sv, "iKeyLimbNext"sv, 0x2C };

		// line of sight: only visible hostiles can be locked, cycled to or chained to; a locked target that stays out of sight for
		// fLosLostSeconds is released. The visibility test is the game's own VATS one, repeated at most every fLosInterval seconds per actor.
		inline REX::TOML::Bool LineOfSight{ "RealtimeVats"sv, "bLineOfSight"sv, true };
		inline REX::TOML::F32 LosLostSeconds{ "RealtimeVats"sv, "fLosLostSeconds"sv, 1.5f };
		inline REX::TOML::F32 LosInterval{ "RealtimeVats"sv, "fLosInterval"sv, 0.2f };

		// when the locked target is killed, lock on to the next closest hostile in view (degrees from the view direction)
		inline REX::TOML::Bool Chain{ "RealtimeVats"sv, "bChain"sv, true };
		inline REX::TOML::F32 ChainMaxAngle{ "RealtimeVats"sv, "fChainMaxAngle"sv, 90.0f };
		inline REX::TOML::F32 ChainMaxRange{ "RealtimeVats"sv, "fChainMaxRange"sv, 1500.0f };

		// action points: holding the lock drains AP slowly, every shot costs the weapon's attack AP times a multiplier,
		// the lock drops at zero and needs a minimum to start
		inline REX::TOML::Bool ApDrain{ "RealtimeVats"sv, "bApDrain"sv, true };
		inline REX::TOML::F32 LockDrainPerSecond{ "RealtimeVats"sv, "fLockDrainPerSecond"sv, 1.5f };
		inline REX::TOML::F32 ShotCostMult{ "RealtimeVats"sv, "fShotCostMult"sv, 0.4f };
		// AP does not regenerate while a lock is held (otherwise it refills between shots and the costs never show)
		inline REX::TOML::Bool NoRegenWhileLocked{ "RealtimeVats"sv, "bNoRegenWhileLocked"sv, true };
		inline REX::TOML::F32 MinApToLock{ "RealtimeVats"sv, "fMinApToLock"sv, 5.0f };

		// highest hit chance a lock-on shot can have (0..1)
		inline REX::TOML::F32 MaxChance{ "RealtimeVats"sv, "fMaxChance"sv, 0.95f };

		// critical hits: hits on the locked target charge a meter; with a full meter the crit key arms the next hit as a critical
		inline REX::TOML::I32 KeyCrit{ "RealtimeVats"sv, "iKeyCrit"sv, 0x21 };
		inline REX::TOML::F32 CritChargePerHit{ "RealtimeVats"sv, "fCritChargePerHit"sv, 0.2f };
		inline REX::TOML::F32 CritDamageMult{ "RealtimeVats"sv, "fCritDamageMult"sv, 3.0f };

		// the player's shots only critical through the meter (no natural or sneak attack crits)
		inline REX::TOML::Bool NoNaturalCrits{ "RealtimeVats"sv, "bNoNaturalCrits"sv, true };

		// while locked, the game's own bindings of the keys above (Grab on Z, Toggle POV on F, ...) are unbound, so a press only acts for the plugin
		inline REX::TOML::Bool FreeGameKeys{ "RealtimeVats"sv, "bFreeGameKeys"sv, true };

		// highlight the aimed limb with the game's own VATS body part effect
		inline REX::TOML::Bool LimbHighlight{ "RealtimeVats"sv, "bLimbHighlight"sv, true };

		// distance (UI units) between the edge of the limb highlight and the info panel
		inline REX::TOML::F32 PanelGap{ "RealtimeVats"sv, "fPanelGap"sv, 60.0f };

		// log the target and hit chance about once a second
		inline REX::TOML::Bool LogChance{ "RealtimeVats"sv, "bLogChance"sv, true };
	}
}

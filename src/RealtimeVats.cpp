#include <numbers>
#include <unordered_map>
#include "RealtimeVats.h"
#include "Settings.h"

namespace Plugin::RealtimeVats
{
	namespace
	{
		// ----------------------------------------------------------------------------------------- game addresses (RVA)

		constexpr const char*   kOverlayXml{ "Data\\Menus\\RealtimeVatsNV.xml" };
		constexpr std::int32_t  kGunSkillVats{ 3 };                  // GUN_SKILL_ENUM used by the VATS chance
		constexpr float         kTorsoFraction{ 0.8f };              // torso aim point: this fraction of the way from the root node to the head node
		constexpr float         kEyeHeight{ 120.0f };                // player eye height above the reference position
		constexpr float         kFrameMin{ 28.0f };                  // size range (UI units) of the limb highlight frame
		constexpr float         kFrameMax{ 160.0f };
		constexpr std::uint32_t kHitCritical{ 0x4 };                 // HitData::uiFlags: HD_CRITICAL

		using UpdateAutoAimActor_t = void(__thiscall*)(RE::PlayerCharacter*);
		using ApplyAutoAimActor_t = void(__thiscall*)(RE::PlayerCharacter*, RE::Projectile*, RE::NiAVObject*, float*, float*, RE::NiPoint3*, float, float);
		using CheckCriticalStrike_t = void(__thiscall*)(RE::HitData*);
		using GetCriticalChance_t = float(__cdecl*)(RE::ActorValueOwner*, RE::TESObjectWEAP*);

		// the original functions behind the hooked calls
		REL::Relocation<UpdateAutoAimActor_t>  _UpdateAutoAimActor;
		REL::Relocation<ApplyAutoAimActor_t>   _ApplyAutoAimActor;
		REL::Relocation<CheckCriticalStrike_t> _CheckCriticalStrike;
		REL::Relocation<GetCriticalChance_t>   _GetCriticalChance;

		// ----------------------------------------------------------------------------------------- state
		struct Candidate
		{
			RE::Actor* actor;
			float      angle;     // radians between the view direction and the target
			float      distance;
		};

		bool          g_wantLock{ false };
		bool          g_wantNext{ false };
		bool          g_wantPrevious{ false };
		bool          g_wantCrit{ false };
		bool          g_wantLimbNext{ false };
		bool          g_wantLimbPrevious{ false };

		// aimed body parts the keys cycle through (BGSBodyPart::LIMB_ENUM): torso, head, left arm, right arm, left leg, right leg
		struct LimbInfo
		{
			std::int32_t index;
			const char*  name;
		};
		constexpr LimbInfo kLimbs[]{ { 0, "Torso" }, { 1, "Head" }, { 3, "Left arm" }, { 5, "Right arm" }, { 7, "Left leg" }, { 10, "Right leg" } };
		std::int32_t  g_limb{ -1 };                // aimed limb; -1 until set from the configuration

		// line of sight: the visibility of an actor is re-tested at most every Settings::LosInterval seconds
		struct SightEntry
		{
			float checkedAt;
			bool  visible;
		};
		std::unordered_map<std::uint32_t, SightEntry> g_sight;
		float         g_clock{ 0.0f };             // seconds of (capped) frame time since the plugin started
		float         g_sightLostAt{ -1.0f };      // g_clock when the locked target went out of sight, negative while it is in sight

		// critical meter: hits on the locked target charge it, a full meter can be armed with the crit key
		float         g_critMeter{ 0.0f };
		bool          g_critArmed{ false };
		bool          g_forcingCrit{ false };   // true while the next critical roll is being forced

		std::uint32_t g_targetID{ 0 };            // form id of the locked actor (0 = none)
		RE::Actor*    g_target{ nullptr };        // resolved every frame from the process list
		RE::NiPoint3  g_aimPoint{};
		float         g_chance{ 0.0f };

		// inputs of the last chance calculation, for the log
		struct ChanceInputs
		{
			std::uint32_t weapon{ 0 };
			bool          bodyPart{ false };
			float         skill{ 0.0f };
			float         reach{ 0.0f };
			float         range{ 0.0f };
			float         size{ 0.0f };
			float         raw{ 0.0f };
		};
		ChanceInputs  g_inputs;
		std::uint32_t g_frame{ 0 };
		std::optional<float> g_lastAp;             // lowest AP seen while locked, regeneration above it is cancelled (AP can go below zero)
		bool          g_shotAffordable{ true };    // the shot of g_lastShotFrame could be paid for
		std::uint32_t g_lastShotFrame{ 0 };       // the frame whose shot has been charged (a shotgun fires many projectiles per shot)
		std::chrono::steady_clock::time_point g_lastTime{};

		std::mt19937 g_rng{ std::random_device{}() };

		// ----------------------------------------------------------------------------------------- helpers
		// the game's own VATS visibility test (rays from the player towards the actor's bound); zero when the actor cannot be seen
		bool Visible(RE::PlayerCharacter* a_player, RE::Actor* a_actor)
		{
			if (!Settings::LineOfSight.GetValue()) {
				return true;
			}

			if (g_sight.size() > 512) {
				g_sight.clear();
			}
			auto& entry = g_sight[a_actor->iFormID];
			if (entry.checkedAt <= 0.0f || g_clock - entry.checkedAt >= Settings::LosInterval.GetValue()) {
				const float offset = a_player->GetVATSTargetVisible(a_actor, 0.0f);
				const bool  visible = offset > 0.0f;
				if (entry.checkedAt <= 0.0f || visible != entry.visible) {
					REX::TRACE("sight {:08X}: {} (visibility offset {:.1f})", a_actor->iFormID, visible ? "visible" : "hidden", offset);
				}
				entry.visible = visible;
				entry.checkedAt = std::max(g_clock, 0.001f);
			}
			return entry.visible;
		}

		std::int32_t CurrentLimb()
		{
			if (g_limb < 0) {
				g_limb = Settings::Limb.GetValue();
			}
			return g_limb;
		}

		const char* LimbName(std::int32_t a_limb)
		{
			for (const auto& limb : kLimbs) {
				if (limb.index == a_limb) {
					return limb.name;
				}
			}
			return "Body";
		}

		// the limb is aimable when the target's body part data has it (the torso always is)
		bool LimbAvailable(RE::Actor* a_actor, std::int32_t a_limb)
		{
			if (a_limb == 0) {
				return true;
			}
			if (const auto base = a_actor->GetActorBase()) {
				if (const auto data = base->GetBodyPartData()) {
					return data->GetBodyPart(static_cast<RE::BGSBodyPart_LIMB_ENUM>(a_limb)) != nullptr;
				}
			}
			return false;
		}

		bool IsMenuMode()
		{
			using enum RE::InterfaceManager::eMenuModeType;

			const auto ui = RE::InterfaceManager::Instance();
			return ui && (ui->cMenuMode == kEMenuModeOn || ui->cMenuMode == kEMenuModeFirstPass);
		}

		bool IsTextInputActive()
		{
			if (RE::Menu::IsMenuVisible(static_cast<std::int32_t>(RE::Interface_enumMenus::kETextEditMenu))) {
				return true;
			}

			const auto console = RE::MenuConsole::Instance(false);
			return console && console->IsVisible();
		}

		// view direction: heading is atan2(x, y) and the pitch is positive when looking down (as in TESObjectWEAP::Fire)
		RE::NiPoint3 ViewDirection(RE::PlayerCharacter* a_player)
		{
			const float heading = a_player->GetHeading(false);
			const float pitch = a_player->GetLooking();
			return { std::sin(heading) * std::cos(pitch), std::cos(heading) * std::cos(pitch), -std::sin(pitch) };
		}

		RE::NiPoint3 EyePosition(RE::PlayerCharacter* a_player)
		{
			auto pos = a_player->data.Location;
			pos.z += kEyeHeight;
			return pos;
		}

		// the limb nodes (BGSBodyPart::LIMB_ENUM index) of a high-process actor
		RE::NiNode* const* LimbNodes(RE::Actor* a_actor)
		{
			return static_cast<RE::MiddleHighProcess*>(a_actor->pCurrentProcess)->pDamageRootNode;
		}

		// world position of a limb node of a high-process actor; falls back to the middle of the body
		RE::NiPoint3 AimPoint(RE::Actor* a_actor, std::int32_t a_limb)
		{
			if (a_limb >= 0 && a_limb < 15 && a_actor->pCurrentProcess) {
				const auto nodes = LimbNodes(a_actor);
				if (const auto node = nodes[a_limb]) {
					auto point = node->m_kWorld.m_Translate;

					// node 0 is the actor's root at the feet; the torso is aimed at on the way to the head node
					if (a_limb == 0) {
						if (const auto head = nodes[1]) {
							point += (head->m_kWorld.m_Translate - point) * kTorsoFraction;
						} else {
							point.z += 70.0f;
						}
					}
					return point;
				}
			}

			auto pos = a_actor->data.Location;
			pos.z += 64.0f;
			return pos;
		}

		// every high-process actor that is alive, hostile and perceived by the player
		std::vector<Candidate> Gather(RE::PlayerCharacter* a_player)
		{
			std::vector<Candidate> result;

			const auto& array = RE::ProcessLists::GetSingleton()->MobProcessArray;
			const auto  count = array.ProcessLevelTails[0];

			const auto eye = EyePosition(a_player);
			const auto view = ViewDirection(a_player);

			for (std::uint32_t i = 0; i < count; ++i) {
				const auto mob = array.GetItem(i);
				if (!mob || mob == static_cast<RE::MobileObject*>(a_player) || !mob->IsActor()) {
					continue;
				}

				const auto actor = static_cast<RE::Actor*>(mob);
				if (actor->IsDead(false)) {
					continue;
				}

				const auto perceived = a_player->IsActorPerceived(actor);
				if (!perceived || !perceived->bHostile) {
					continue;
				}

				const auto toTarget = AimPoint(actor, 0) - eye;
				const float distance = toTarget.Length();
				if (distance < 1.0f || distance > Settings::MaxRange.GetValue()) {
					continue;
				}

				const float dot = toTarget.Dot(view) / distance;
				result.push_back({ actor, std::acos(std::clamp(dot, -1.0f, 1.0f)), distance });
			}

			std::ranges::sort(result, [](const Candidate& a, const Candidate& b) { return a.angle < b.angle; });
			return result;
		}

		// hit chance (0..1) of the current weapon against a limb of the target, from the engine's VATS formula
		float ComputeChance(RE::PlayerCharacter* a_player, RE::Actor* a_target, std::int32_t a_limb)
		{
			const auto weapon = a_player->GetCurrentWeapon();

			RE::BGSBodyPart* bodyPart{ nullptr };
			if (const auto base = a_target->GetActorBase()) {
				if (const auto data = base->GetBodyPartData()) {
					bodyPart = data->GetBodyPart(static_cast<RE::BGSBodyPart_LIMB_ENUM>(a_limb));
				}
			}

			float size = a_target->GetModelBoundSize() * 2.0f;
			if (!(size > 0.0f)) {
				size = 100.0f;
			}

			const float skill = a_player->GetGunSkill(static_cast<RE::GUN_SKILL_ENUM>(kGunSkillVats));
			const float reach = RE::CombatUtilities::GetWeaponReach(a_player, weapon);
			const float range = RE::CombatUtilities::GetDistanceBetweenFacingActors(a_player, a_target, false);

			const float chance = RE::CombatFormulas::GetVATSChanceToHit(
				static_cast<RE::ActorValueOwner*>(a_player), a_target, weapon,
				1.0f, reach, skill, bodyPart, nullptr, 1.0f, range, size, nullptr);

			g_inputs = { weapon ? weapon->iFormID : 0, bodyPart != nullptr, skill, reach, range, size, chance };

			return std::clamp(chance, 0.0f, Settings::MaxChance.GetValue());
		}

		// a target can be locked, cycled to or chained to when it is visible and its hit chance reaches the minimum
		bool Eligible(RE::PlayerCharacter* a_player, RE::Actor* a_actor)
		{
			return Visible(a_player, a_actor) && ComputeChance(a_player, a_actor, CurrentLimb()) >= Settings::MinChanceToLock.GetValue();
		}

		// HUD message (the game ignores it until the HUD exists)
		void Notify(const std::string& a_text, float a_seconds)
		{
			RE::Interface::ShowMessage(
				a_text.c_str(), 0, "Interface\\Icons\\Message Icons\\glow_message_vaultboy_neutral.dds", nullptr, a_seconds, true);
		}

		std::string TargetName()
		{
			if (g_target) {
				if (const auto base = g_target->GetActorBase()) {
					if (const auto name = RE::TESFullName::GetFullName(static_cast<RE::TESForm*>(base)); name && *name) {
						return name;
					}
				}
			}
			return "target";
		}

		// ----------------------------------------------------------------------------------------- game bindings
		// The game reads its own bindings (Grab on Z, Toggle POV on F, ...) from the same keys: a press would also act in the game.
		// While a lock is held, every user action bound to one of the plugin's keys is unbound in the controls object and bound again
		// afterwards. The object (singleton at 0x11F35CC) keeps one byte per user action and device, 0 keyboard and 1 mouse, 28 bytes
		// per device from offset 0x1B94 (the table Controls::GetUserActionState 0xA24660 reads); 0xFF is unbound.
		constexpr std::size_t   kBindingsOffset{ 0x1B94 };
		constexpr std::size_t   kBindingsPerDevice{ 28 };
		constexpr std::uint8_t  kUnbound{ 0xFF };

		struct FreedBinding
		{
			std::size_t  action;
			std::uint8_t scanCode;
		};
		std::vector<FreedBinding> g_freed;

		std::uint8_t* KeyboardBindings()
		{
			static const REL::Relocation<std::uint8_t**> controls{ REL::Offset{ 0xDF35CC } };
			const auto                                   base = *controls;
			return base ? base + kBindingsOffset : nullptr;
		}

		void RestoreGameKeys()
		{
			if (g_freed.empty()) {
				return;
			}
			if (const auto bindings = KeyboardBindings()) {
				for (const auto& freed : g_freed) {
					bindings[freed.action] = freed.scanCode;
				}
			}
			REX::DEBUG("game bindings restored ({})", g_freed.size());
			g_freed.clear();
		}

		void FreeGameKeys()
		{
			if (!g_freed.empty()) {
				return;
			}
			const auto bindings = KeyboardBindings();
			if (!bindings) {
				return;
			}

			const std::int32_t ours[]{ Settings::KeyLock.GetValue(), Settings::KeyNext.GetValue(), Settings::KeyPrevious.GetValue(),
				Settings::KeyCrit.GetValue(), Settings::KeyLimbNext.GetValue(), Settings::KeyLimbPrevious.GetValue() };
			for (std::size_t action = 0; action < kBindingsPerDevice; ++action) {
				const auto code = bindings[action];
				if (code == kUnbound || std::ranges::find(ours, static_cast<std::int32_t>(code)) == std::end(ours)) {
					continue;
				}
				g_freed.push_back({ action, code });
				bindings[action] = kUnbound;
				REX::DEBUG("game action {} freed from key {:#x} while locked", action, code);
			}
		}

		// per main loop pass (also in menus): free the keys while locked in the game world, restore them everywhere else
		void SyncGameKeys()
		{
			if (g_targetID != 0 && g_target && Settings::Enabled.GetValue() && Settings::FreeGameKeys.GetValue() && !IsMenuMode()) {
				FreeGameKeys();
			} else {
				RestoreGameKeys();
			}
		}

		// ----------------------------------------------------------------------------------------- limb highlight
		// The engine's own VATS highlight (FOVATSEffectManager): the target is drawn into a mask with only the chosen body part
		// enabled and the image space effect tints and scans it. The HighlightBodyPart / DeactivateAllHighlights script commands
		// drive it the same way outside the VATS menu: SetTarget(reference, limb), then the single target mode.
		RE::TESObjectREFR* g_highlightRef{ nullptr };
		std::int32_t       g_highlightLimb{ -1 };

		bool SetHighlight(RE::TESObjectREFR* a_ref, std::int32_t a_limb)
		{
			const auto effect = RE::Interface::GetVATSEffect();
			if (!effect) {
				return false;
			}

			if (a_ref == g_highlightRef && a_limb == g_highlightLimb && effect->SingleTarget.pRef == a_ref) {
				return true;
			}

			effect->SetTarget(a_ref, static_cast<RE::BGSBodyPart_LIMB_ENUM>(a_ref ? a_limb : 0), false);
			if (!a_ref) {
				effect->ClearMultiTargets();
			}
			effect->SetEffectMode(RE::FOVATSEffectManager::VATS_EFFECT_MODE::kSINGLE_TARGET);

			g_highlightRef = a_ref;
			g_highlightLimb = a_limb;
			return true;
		}

		// ----------------------------------------------------------------------------------------- overlay
		float g_uiWidth{ 0.0f };    // size of the tile tree's coordinate space
		float g_uiHeight{ 0.0f };
		float g_screenX{ -1.0f };   // last projection of the aim point (port coordinates), -1 when not on screen
		float g_screenY{ -1.0f };
		bool g_overlayFailed{ false };
		bool g_overlayShown{ false };

		// Tile::LoadFromXML (0x601B00) without its last step: it destroys the parsed template tree, which crashes the game for a
		// menu without a class (the tiles are built and connected by then). The parse result is kept instead (a few KB, once).
		RE::Tile* LoadXml(RE::Tile* a_parent, const char* a_path)
		{
			auto& error = RE::Tile::ParseError();

			// the interface memory context while the tiles are allocated
			RE::AutoMemContext memoryContext;
			memoryContext.Enter(RE::MEM_CONTEXT::kMEM_CONTEXT_INTERFACE, false, nullptr, 0);

			RE::Tile* tile{ nullptr };
			error = false;

			const auto storage = RE::Tile::ParseFile(a_path, nullptr, 0);
			if (storage && !error) {
				const auto tmpl = storage->pCurrentTemplate ? storage->pCurrentTemplate : storage->pTemplate;  // the alternate root when there is one

				tile = a_parent->BuildAndNameTree(tmpl);
				if (tile && !error) {
					tile->ConnectTraitsToTree(tmpl);
				}
				if (error) {
					tile = nullptr;
				}
				if (tile) {
					const auto lock = &RE::NiRenderer::GetSingleton()->m_kRendererLock.m_kCriticalSection;
					::EnterCriticalSection(lock);
					tile->UpdateChildren(false);
					::LeaveCriticalSection(lock);
				}
			}

			error = false;
			memoryContext.Leave();
			return tile;
		}

		RE::Tile* OverlayRoot(bool a_load)
		{
			const auto ui = RE::InterfaceManager::Instance();
			if (!ui) {
				return nullptr;
			}

			const auto root = ui->pMenusRoot;
			if (!root) {
				return nullptr;
			}

			const auto child = [](RE::Tile* a_tile, const char* a_name) { return a_tile->GetChildByName(a_name); };
			if (const auto tile = child(root, "RealtimeVatsNVMenu")) {
				return tile;
			}

			if (!a_load || g_overlayFailed) {
				return nullptr;
			}

			// the game crashes while cleaning up after a failed parse, so only load a file that exists
			std::error_code ec;
			if (!std::filesystem::is_regular_file(kOverlayXml, ec)) {
				g_overlayFailed = true;
				REX::ERROR("overlay: {} not found (current directory {}), the HUD overlay is disabled (HUD messages only)", kOverlayXml, std::filesystem::current_path(ec).string());
				return nullptr;
			}

			REX::DEBUG("overlay: loading {}", kOverlayXml);
			const auto tile = LoadXml(root, kOverlayXml);
			if (!tile) {
				g_overlayFailed = true;
				REX::ERROR("overlay: loading {} failed, the HUD overlay is disabled (HUD messages only)", kOverlayXml);
			} else {
				REX::DEBUG("overlay: loaded {}", kOverlayXml);
			}
			return tile;
		}

		void SetVisible(RE::Tile* a_tile, bool a_visible)
		{
			static const auto trait = RE::Tile::TextToTrait("visible");
			a_tile->Set(trait, a_visible ? 1.0f : 0.0f, true);
		}

		void HideOverlay()
		{
			if (!g_overlayShown) {
				return;
			}
			g_overlayShown = false;

			if (const auto root = OverlayRoot(false)) {
				for (const auto name : { "RTV_Marker", "RTV_Panel" }) {
					if (const auto tile = root->GetChildByName(name)) {
						SetVisible(tile, false);
					}
				}
			}
		}

		// approximate radius (game units) of the aimed limb, for the size of the highlight
		float LimbRadius(std::int32_t a_limb)
		{
			switch (a_limb) {
			case 0:
				return 28.0f;
			case 1:
				return 12.0f;
			case 3:
			case 5:
				return 10.0f;
			case 7:
			case 10:
				return 14.0f;
			default:
				return 16.0f;
			}
		}

		// highlight frame over the aimed limb, and the info panel beside it (away from the body)
		void UpdateOverlay()
		{
			const auto root = OverlayRoot(true);
			if (!root) {
				return;
			}

			const auto child = [](RE::Tile* a_tile, const char* a_name) { return a_tile->GetChildByName(a_name); };
			const auto marker = child(root, "RTV_Marker");
			const auto panel = child(root, "RTV_Panel");
			if (!marker || !panel) {
				return;
			}

			float nx{ 0.0f };
			float ny{ 0.0f };
			float topY{ 0.0f };   // projection of a point one limb radius above the aim point
			bool  onScreen{ false };
			if (const auto graph = RE::Main::GetWorldSceneGraph()) {
				if (const auto camera = graph->spCamera.get()) {
					onScreen = camera->WorldPtToScreenPt(g_aimPoint, nx, ny, 1e-5f) && nx >= 0.0f && nx <= 1.0f && ny >= 0.0f && ny <= 1.0f;
					if (onScreen) {
						float topX{ 0.0f };
						auto  top = g_aimPoint;
						top.z += LimbRadius(CurrentLimb());
						if (!camera->WorldPtToScreenPt(top, topX, topY, 1e-5f)) {
							topY = ny;
						}
					}
				}
			}

			g_screenX = onScreen ? nx : -1.0f;
			g_screenY = onScreen ? ny : -1.0f;

			if (!onScreen) {
				HideOverlay();
				return;
			}

			static const auto traitX = RE::Tile::TextToTrait("x");
			static const auto traitY = RE::Tile::TextToTrait("y");
			static const auto traitString = RE::Tile::TextToTrait("string");

			// the frustum point has its origin at the bottom left, the tile tree at the top left
			// the tile tree has its own coordinate space: the size of the interface root tile, not the render target
			static const auto traitWidth = RE::Tile::TextToTrait("width");
			static const auto traitHeight = RE::Tile::TextToTrait("height");
			const auto uiRoot = RE::InterfaceManager::Instance()->pMenusRoot;
			const auto getFloat = [](RE::Tile* a_tile, std::int32_t a_trait) { return a_tile->GetFloat(a_trait); };
			float      width = uiRoot ? getFloat(uiRoot, traitWidth) : 0.0f;
			float      height = uiRoot ? getFloat(uiRoot, traitHeight) : 0.0f;
			if (!(width > 0.0f) || !(height > 0.0f)) {
				width = RE::InterfaceManager::GetRealScreenWidth();
				height = RE::InterfaceManager::GetRealScreenHeight();
			}
			g_uiWidth = width;
			g_uiHeight = height;

			const auto setFloat = [](RE::Tile* a_tile, std::int32_t a_trait, float a_value, bool a_clear) { a_tile->Set(a_trait, a_value, a_clear); };
			const float aimX = nx * width;
			const float aimY = (1.0f - ny) * height;

			// highlight: a frame as large as the limb appears on the screen
			const float frameSize = std::clamp(std::abs(topY - ny) * height * 2.0f, kFrameMin, kFrameMax);
			setFloat(marker, traitX, aimX, true);
			setFloat(marker, traitY, aimY, true);
			if (const auto frame = child(marker, "RTV_Frame")) {
				SetVisible(frame, !g_highlightRef);   // the engine highlight shows the limb; the frame is only the fallback
				setFloat(frame, traitWidth, frameSize, true);
				setFloat(frame, traitHeight, frameSize, true);
				setFloat(frame, traitX, -frameSize / 2.0f, true);
				setFloat(frame, traitY, -frameSize / 2.0f, true);
			}

			// panel: beside the highlight, on the side of the screen with more room, so it stays off the body
			const bool  panelRight = nx < 0.5f;
			const float gap = frameSize / 2.0f + Settings::PanelGap.GetValue();
			setFloat(panel, traitX, aimX + (panelRight ? gap : -gap), true);
			setFloat(panel, traitY, aimY, true);

			if (const auto text = child(panel, "RTV_Crit")) {
				std::string label;
				if (g_critArmed) {
					label = "CRIT ARMED";
				} else if (g_critMeter >= 1.0f) {
					label = "CRIT READY";
				} else if (g_critMeter > 0.0f) {
					label = std::format("CRIT {:.0f}%", g_critMeter * 100.0f);
				} else {
					label = " ";
				}
				text->Set(traitString, label.c_str(), true);
			}

			// the VATS-style callout (the game's body part template): the bracket points at the highlight and opens to the side
			// with room; name, chance and a 70 px meter hang on it
			{
				struct Callout
				{
					const char* name;
					float       meterLeft;  // left edge of the meter
					int         justify;    // meter fill: 0 grows from the left, 1 from the middle, 2 from the right
				};
				static constexpr Callout callouts[]{ { "RTV_Right", -49.0f, 0 }, { "RTV_Left", -21.0f, 2 } };
				constexpr float          kMeterWidth{ 70.0f };

				const auto        limb = CurrentLimb();
				const std::size_t active = panelRight ? 0 : 1;
				for (std::size_t i = 0; i < std::size(callouts); ++i) {
					const auto callout = child(panel, callouts[i].name);
					if (!callout) {
						continue;
					}
					SetVisible(callout, i == active);
					if (i != active) {
						continue;
					}

					if (const auto text = child(callout, "RTV_Name")) {
						text->Set(traitString, LimbName(limb), true);
					}
					if (const auto text = child(callout, "RTV_Chance")) {
						const auto label = std::format("{:.0f}%", g_chance * 100.0f);
						text->Set(traitString, label.c_str(), true);
					}
					if (const auto fill = child(callout, "RTV_MeterFill")) {
						const float fillWidth = std::max(1.0f, kMeterWidth * std::clamp(g_chance, 0.0f, 1.0f));
						const float offset = callouts[i].justify == 0 ? 0.0f : (callouts[i].justify == 1 ? (kMeterWidth - fillWidth) / 2.0f : kMeterWidth - fillWidth);
						setFloat(fill, traitWidth, fillWidth, true);
						setFloat(fill, traitX, callouts[i].meterLeft + offset, true);
					}
				}
			}

			// critical meter bar
			if (const auto fill = child(panel, "RTV_CritFill")) {
				const float meter = g_critArmed ? 1.0f : std::clamp(g_critMeter, 0.0f, 1.0f);
				setFloat(fill, traitWidth, std::max(1.0f, 100.0f * meter), true);
			}

			if (const auto text = child(panel, "RTV_Target")) {
				text->Set(traitString, TargetName().c_str(), true);
			}

			// the target's damage threshold and resistance
			if (const auto text = child(panel, "RTV_Resist")) {
				const auto avo = static_cast<RE::ActorValueOwner*>(g_target);
				const auto label = std::format("DT {:.0f}  DR {:.0f}%", avo->GetActorFloatValue(RE::ActorValue_Index::kDAMAGE_THRESHOLD), avo->GetActorFloatValue(RE::ActorValue_Index::kDAMAGE_RESISTANCE));
				text->Set(traitString, label.c_str(), true);
			}

			SetVisible(marker, true);
			SetVisible(panel, true);
			g_overlayShown = true;
		}

		void UseAp(RE::PlayerCharacter* a_player, float a_amount)
		{
			a_player->UseActionPoints(a_amount);
		}

		float ActionPoints(RE::PlayerCharacter* a_player)
		{
			return static_cast<RE::ActorValueOwner*>(static_cast<RE::Actor*>(a_player))->GetActorFloatValue(RE::ActorValue_Index::kACTION_POINTS);
		}

		// every reading of the action points the actor value owner offers, for working out which one is the current value
		std::string ApReadings(RE::PlayerCharacter* a_player)
		{
			const auto avo = static_cast<RE::ActorValueOwner*>(static_cast<RE::Actor*>(a_player));
			const auto ap = RE::ActorValue_Index::kACTION_POINTS;
			return std::format("base {:.1f}, value(int) {}, value(float) {:.1f}, temp {:.1f}, damage {:.1f}, permanent {:.1f}, permanent value {:.1f}",
				avo->GetBaseActorFloatValue(ap), avo->GetActorValue(ap), avo->GetActorFloatValue(ap), avo->GetTemporaryModifier(ap), avo->GetDamageModifier(ap), avo->GetPermanentModifier(ap), avo->GetPermanentActorFloatValue(ap));
		}

		bool IsGodMode()
		{
			return RE::PlayerCharacter::IsGodMode();
		}

		void Release(const char* a_why)
		{
			if (g_targetID != 0) {
				REX::DEBUG("lock-on released ({})", a_why);
				Notify("Lock-on released", 1.0f);
			}
			HideOverlay();
			SetHighlight(nullptr, 0);
			RestoreGameKeys();
			g_targetID = 0;
			g_target = nullptr;
			g_chance = 0.0f;
			g_critArmed = false;
			g_lastAp.reset();
			g_sightLostAt = -1.0f;
		}

		void Acquire(RE::Actor* a_actor)
		{
			g_targetID = a_actor->iFormID;
			g_target = a_actor;
			REX::DEBUG("lock-on acquired: form {:08X}", g_targetID);
			if (Settings::ApDrain.GetValue() && IsGodMode()) {
				REX::WARN("god mode is on: the game ignores action point use, the AP costs have no effect (toggle it off with tgm)");
			}
		}

		// Gameplay keys are not read through InterfaceManager::Idle, so they are polled: DirectInput scan code -> virtual key,
		// edge detected once per frame, only while the game window has the focus.
		bool KeyPressed(std::int32_t a_dik, bool& a_wasDown)
		{
			const auto vk = ::MapVirtualKeyW(static_cast<UINT>(a_dik) & 0xFF, MAPVK_VSC_TO_VK_EX);
			DWORD     pid{ 0 };
			::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
			const bool down = vk != 0 && pid == ::GetCurrentProcessId() && (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
			const bool pressed = down && !a_wasDown;
			a_wasDown = down;
			return pressed;
		}

		void PollKeys()
		{
			static bool lockDown{ false };
			static bool nextDown{ false };
			static bool previousDown{ false };
			static bool critDown{ false };
			static bool limbNextDown{ false };
			static bool limbPreviousDown{ false };

			const bool typing = IsTextInputActive();
			const bool lock = KeyPressed(Settings::KeyLock.GetValue(), lockDown);
			const bool next = KeyPressed(Settings::KeyNext.GetValue(), nextDown);
			const bool previous = KeyPressed(Settings::KeyPrevious.GetValue(), previousDown);
			const bool crit = KeyPressed(Settings::KeyCrit.GetValue(), critDown);
			const bool limbNext = KeyPressed(Settings::KeyLimbNext.GetValue(), limbNextDown);
			const bool limbPrevious = KeyPressed(Settings::KeyLimbPrevious.GetValue(), limbPreviousDown);
			if (typing) {
				return;
			}

			g_wantLock = g_wantLock || lock;
			g_wantNext = g_wantNext || next;
			g_wantPrevious = g_wantPrevious || previous;
			g_wantCrit = g_wantCrit || crit;
			g_wantLimbNext = g_wantLimbNext || limbNext;
			g_wantLimbPrevious = g_wantLimbPrevious || limbPrevious;
		}

		// per frame, after the engine's own auto-aim update
		void Update(RE::PlayerCharacter* a_player)
		{
			++g_frame;

			const auto  now = std::chrono::steady_clock::now();
			const float dt = g_lastTime.time_since_epoch().count() == 0 ? 0.0f : std::min(0.1f, std::chrono::duration<float>(now - g_lastTime).count());
			g_lastTime = now;
			g_clock += dt;

			if (Settings::Enabled.GetValue() && !IsMenuMode()) {
				PollKeys();
			}

			const bool lock = std::exchange(g_wantLock, false);
			const bool next = std::exchange(g_wantNext, false);
			const bool previous = std::exchange(g_wantPrevious, false);
			const bool crit = std::exchange(g_wantCrit, false);
			const bool limbNext = std::exchange(g_wantLimbNext, false);
			const bool limbPrevious = std::exchange(g_wantLimbPrevious, false);

			if (!Settings::Enabled.GetValue() || IsMenuMode()) {
				return;
			}

			if (lock && g_targetID != 0) {
				Release("key");
				return;
			}

			if (lock || next || previous || g_targetID != 0) {
				const auto candidates = Gather(a_player);

				// resolve the locked actor again: a form id is safe across cell changes and deaths, a pointer is not
				g_target = nullptr;
				std::size_t index = candidates.size();
				for (std::size_t i = 0; i < candidates.size(); ++i) {
					if (candidates[i].actor->iFormID == g_targetID) {
						g_target = candidates[i].actor;
						index = i;
						break;
					}
				}

				if (g_targetID != 0 && !g_target) {
					// a killed target hands the lock to the next hostile in view; any other loss just releases it
					bool dead = false;
					if (const auto form = RE::TESForm::GetFormByNumericID(g_targetID); form && form->IsActor()) {
						dead = static_cast<RE::Actor*>(form)->IsDead(false);
					}

					// the next target is the closest hostile within the chain cone and range
					const float chainAngle = Settings::ChainMaxAngle.GetValue() * std::numbers::pi_v<float> / 180.0f;
					const Candidate* chainTarget{ nullptr };
					if (dead && Settings::Chain.GetValue()) {
						std::vector<const Candidate*> inCone;
						for (const auto& candidate : candidates) {
							if (candidate.angle <= chainAngle && candidate.distance <= Settings::ChainMaxRange.GetValue()) {
								inCone.push_back(&candidate);
							}
						}

						// the closest one that is visible and has a high enough chance (the checks stop at the first)
						std::ranges::sort(inCone, [](const Candidate* a, const Candidate* b) { return a->distance < b->distance; });
						const auto closest = std::ranges::find_if(inCone, [&](const Candidate* a_candidate) { return Eligible(a_player, a_candidate->actor); });
						chainTarget = closest != inCone.end() ? *closest : nullptr;
					}

					if (chainTarget) {
						REX::DEBUG("chain: {:08X} killed, next target {:08X} at {:.0f} units", g_targetID, chainTarget->actor->iFormID, chainTarget->distance);
						Acquire(chainTarget->actor);
						g_target = chainTarget->actor;
						index = static_cast<std::size_t>(chainTarget - candidates.data());
					} else {
						Release(dead ? "target killed" : "target lost");
					}
				}

				const float maxAngle = Settings::MaxAngle.GetValue() * std::numbers::pi_v<float> / 180.0f;
				if (lock && g_targetID == 0 && Settings::ApDrain.GetValue() && ActionPoints(a_player) < Settings::MinApToLock.GetValue()) {
					Notify("Not enough AP to lock on", 1.0f);
				} else if (lock && g_targetID == 0) {
					std::size_t pick = candidates.size();
					for (std::size_t i = 0; i < candidates.size() && candidates[i].angle <= maxAngle; ++i) {
						if (Eligible(a_player, candidates[i].actor)) {
							pick = i;
							break;
						}
					}

					if (pick < candidates.size()) {
						Acquire(candidates[pick].actor);
						g_target = candidates[pick].actor;
						index = pick;
					} else if (candidates.empty()) {
						REX::DEBUG("lock key pressed: no hostile, perceived actor within {:.0f} units", Settings::MaxRange.GetValue());
					} else if (candidates.front().angle > maxAngle) {
						REX::DEBUG("lock key pressed: nearest hostile is {:.0f} degrees off the view (limit {:.0f})", candidates.front().angle * 180.0f / std::numbers::pi_v<float>, Settings::MaxAngle.GetValue());
					} else {
						REX::DEBUG("lock key pressed: no hostile in the view cone is visible with a hit chance of at least {:.0f}%", Settings::MinChanceToLock.GetValue() * 100.0f);
					}
				} else if ((next || previous) && !candidates.empty()) {
					// the next (previous) visible candidate after the locked one, wrapping around
					const std::size_t size = candidates.size();
					const std::size_t from = index < size ? index : 0;
					for (std::size_t step = 1; step <= size; ++step) {
						const std::size_t to = next ? (from + step) % size : (from + size - step % size) % size;
						if (candidates[to].actor->iFormID != g_targetID && Eligible(a_player, candidates[to].actor)) {
							Acquire(candidates[to].actor);
							g_target = candidates[to].actor;
							break;
						}
					}
				}
			}

			if (!g_target) {
				return;
			}

			// a locked target that stays out of sight is released after a grace period
			if (Settings::LineOfSight.GetValue()) {
				if (Visible(a_player, g_target)) {
					g_sightLostAt = -1.0f;
				} else if (g_sightLostAt < 0.0f) {
					g_sightLostAt = g_clock;
				} else if (g_clock - g_sightLostAt >= Settings::LosLostSeconds.GetValue()) {
					Release("line of sight lost");
					return;
				}
			}

			// the aimed limb: [ and ] cycle through the ones the target has
			if (limbNext || limbPrevious) {
				const std::int32_t count = static_cast<std::int32_t>(std::size(kLimbs));
				std::int32_t       at = 0;
				for (std::int32_t i = 0; i < count; ++i) {
					if (kLimbs[i].index == CurrentLimb()) {
						at = i;
					}
				}
				for (std::int32_t step = 1; step <= count; ++step) {
					const auto& candidate = kLimbs[((at + (limbNext ? step : -step)) % count + count) % count];
					if (LimbAvailable(g_target, candidate.index)) {
						g_limb = candidate.index;
						break;
					}
				}
				REX::DEBUG("limb: {}", LimbName(g_limb));
			}
			if (!LimbAvailable(g_target, CurrentLimb())) {
				g_limb = 0;   // the new target has no such part
			}

			if (crit) {
				REX::DEBUG("crit key pressed: meter {:.0f}%, armed {}", g_critMeter * 100.0f, g_critArmed);
				if (g_critArmed) {
					g_critArmed = false;
					Notify("Critical disarmed", 1.0f);
				} else if (g_critMeter >= 1.0f) {
					g_critArmed = true;
					Notify("Critical armed: the next hit is a critical", 1.0f);
				}
			}

			if (Settings::ApDrain.GetValue()) {
				if (ActionPoints(a_player) <= 0.0f) {
					Notify("Out of AP", 1.0f);
					Release("out of AP");
					return;
				}
				// the game refills AP within a second or two, so regeneration is cancelled while the lock is held
				const float ap = ActionPoints(a_player);
				if (Settings::NoRegenWhileLocked.GetValue()) {
					if (!g_lastAp) {
						g_lastAp = ap;
					} else if (ap > *g_lastAp + 0.001f) {
						UseAp(a_player, ap - *g_lastAp);
					}
				}

				UseAp(a_player, Settings::LockDrainPerSecond.GetValue() * dt);
				g_lastAp = ActionPoints(a_player);
			}

			const auto limb = CurrentLimb();
			g_aimPoint = AimPoint(g_target, limb);
			g_chance = ComputeChance(a_player, g_target, limb);

			// the engine's auto-aim and the HUD follow the locked actor
			a_player->pAutoAimActor = g_target;
			a_player->pPlayersTargetActor = g_target;
			a_player->BulletAutoAim = g_aimPoint;

			if (Settings::LimbHighlight.GetValue()) {
				SetHighlight(g_target, limb);
			}
			UpdateOverlay();

			if (Settings::LogChance.GetValue() && g_frame % 60 == 0) {
				// positions for checking the projection: player, target reference, every limb node of the target
				std::string limbs;
				if (g_target->pCurrentProcess) {
					const auto nodes = LimbNodes(g_target);
					for (int i = 0; i < 15; ++i) {
						if (nodes[i]) {
							const auto& t = nodes[i]->m_kWorld.m_Translate;
							limbs += std::format(" [{}] {:.0f},{:.0f},{:.0f}", i, t.x, t.y, t.z);
						}
					}
				}
				const auto& pp = a_player->data.Location;
				const auto& tp = g_target->data.Location;
				REX::TRACE("debug: player {:.0f},{:.0f},{:.0f} heading {:.3f} pitch {:.3f}; target ref {:.0f},{:.0f},{:.0f}; port {:.3f},{:.3f}; ui {:.0f}x{:.0f} (real {:.0f}x{:.0f}); limbs{}",
					pp.x, pp.y, pp.z, a_player->GetHeading(false), a_player->GetLooking(),
					tp.x, tp.y, tp.z, g_screenX, g_screenY, g_uiWidth, g_uiHeight,
					RE::InterfaceManager::GetRealScreenWidth(), RE::InterfaceManager::GetRealScreenHeight(), limbs);
			}

			if (Settings::LogChance.GetValue() && g_frame % 60 == 0) {
				REX::TRACE("lock-on target {:08X}: AP {:.1f}, chance {:.1f}% (raw {:.3f}), weapon {:08X}, limb part {}, skill {:.1f}, reach {:.0f}, range {:.0f}, size {:.0f}, aim {:.0f}, {:.0f}, {:.0f}",
					g_targetID, ActionPoints(a_player), g_chance * 100.0f, g_inputs.raw, g_inputs.weapon, g_inputs.bodyPart, g_inputs.skill, g_inputs.reach, g_inputs.range, g_inputs.size, g_aimPoint.x, g_aimPoint.y, g_aimPoint.z);
			}
		}

		// ----------------------------------------------------------------------------------------- hooks
		// Every hit on an actor rolls its critical here. The player's rolls are decided by the meter: a hit on the locked target
		// charges it, an armed critical is forced (and boosted), and everything else cannot critical.
		float __cdecl Hook_GetCriticalChance(RE::ActorValueOwner* a_actor, RE::TESObjectWEAP* a_weapon)
		{
			return g_forcingCrit ? 1000.0f : _GetCriticalChance(a_actor, a_weapon);
		}

		void __fastcall Hook_CheckCriticalStrike(RE::HitData* a_hit, void*)
		{
			const auto player = static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton());
			if (!player || a_hit->pAggressor != player || !Settings::Enabled.GetValue()) {
				_CheckCriticalStrike(a_hit);
				return;
			}

			const bool onLocked = g_target && a_hit->pTarget == g_target;
			if (Settings::LogChance.GetValue()) {
				REX::TRACE("player hit on {:08X} (locked {:08X}, on locked target: {}, armed: {})", a_hit->pTarget ? a_hit->pTarget->iFormID : 0, g_targetID, onLocked, g_critArmed);
			}

			if (onLocked && g_critArmed) {
				g_forcingCrit = true;
				_CheckCriticalStrike(a_hit);
				g_forcingCrit = false;

				if (a_hit->uiFlags & kHitCritical) {
					a_hit->fHealthDamage *= Settings::CritDamageMult.GetValue();
					REX::DEBUG("critical hit on {:08X}: damage {:.1f}", g_targetID, a_hit->fHealthDamage);
					Notify("Critical hit!", 1.0f);
				}
				g_critArmed = false;
				g_critMeter = 0.0f;
				return;
			}

			if (onLocked && g_critMeter < 1.0f) {
				g_critMeter = std::min(1.0f, g_critMeter + Settings::CritChargePerHit.GetValue());
				REX::DEBUG("hit on {:08X}: critical meter {:.0f}%", g_targetID, g_critMeter * 100.0f);
				if (g_critMeter >= 1.0f) {
					Notify("Critical ready", 1.0f);
				}
			}

			if (!Settings::NoNaturalCrits.GetValue()) {
				_CheckCriticalStrike(a_hit);
			}
		}

		void __fastcall Hook_UpdateAutoAimActor(RE::PlayerCharacter* a_this, void*)
		{
			_UpdateAutoAimActor(a_this);
			Update(a_this);
		}

		// Projectile::AimProjectile hands every new projectile to the engine's auto-aim. With a lock the shot is decided here:
		// a roll against the hit chance either points the projectile at the locked limb or leaves the unassisted aim.
		void __fastcall Hook_ApplyAutoAimActor(RE::PlayerCharacter* a_this, void*, RE::Projectile* a_projectile, RE::NiAVObject* a_release, float* a_heading, float* a_looking, RE::NiPoint3* a_origin, float a_startHeading, float a_startLooking)
		{
			const bool assist = g_target && a_projectile && a_projectile->pShooter == static_cast<RE::TESObjectREFR*>(a_this) && a_heading && a_looking && a_origin;
			const float heading = assist ? *a_heading : 0.0f;
			const float looking = assist ? *a_looking : 0.0f;

			_ApplyAutoAimActor(a_this, a_projectile, a_release, a_heading, a_looking, a_origin, a_startHeading, a_startLooking);

			if (!assist) {
				return;
			}

			// one charge per shot, whatever the number of projectiles
			// a shot the player cannot pay for is unassisted and costs nothing
			if (Settings::ApDrain.GetValue()) {
				if (g_frame != g_lastShotFrame) {
					g_lastShotFrame = g_frame;
					g_shotAffordable = true;
					if (const auto weapon = a_this->GetCurrentWeapon()) {
						const float cost = RE::ActionPoints::GetWeaponAttackActionPoints(weapon, false, false) * Settings::ShotCostMult.GetValue();
						if (ActionPoints(a_this) < cost) {
							g_shotAffordable = false;
							REX::DEBUG("shot needs {:.1f} AP, {:.1f} AP left: unassisted", cost, ActionPoints(a_this));
							Notify("Not enough AP for an assisted shot", 1.0f);
						} else {
							const auto before = ApReadings(a_this);
							UseAp(a_this, cost);
							REX::TRACE("shot cost {:.1f} AP, {:.1f} AP left (god mode: {}); before: {}; after: {}", cost, ActionPoints(a_this), IsGodMode(), before, ApReadings(a_this));
						}
					}
				}

				if (!g_shotAffordable) {
					*a_heading = heading;
					*a_looking = looking;
					return;
				}
			}

			const float roll = std::uniform_real_distribution<float>{ 0.0f, 1.0f }(g_rng);
			if (Settings::LogChance.GetValue()) {
				REX::TRACE("shot at {:08X}: roll {:.2f} vs chance {:.2f} -> {}", g_targetID, roll, g_chance, roll < g_chance ? "assisted" : "unassisted");
			}

			if (roll < g_chance) {
				const auto direction = g_aimPoint - *a_origin;
				const float length = direction.Length();
				if (length > 1.0f) {
					*a_heading = std::atan2(direction.x, direction.y);
					*a_looking = -std::asin(std::clamp(direction.z / length, -1.0f, 1.0f));
				}
			} else {
				*a_heading = heading;
				*a_looking = looking;
			}
		}
	}

	void OnMessage(NVSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case NVSE::MessagingInterface::kMainGameLoop:
			SyncGameKeys();
			break;
		case NVSE::MessagingInterface::kExitGame:
		case NVSE::MessagingInterface::kExitGameConsole:
		case NVSE::MessagingInterface::kExitToMainMenu:
		case NVSE::MessagingInterface::kSaveGame:
			RestoreGameKeys();
			break;
		default:
			break;
		}
	}

	void Install()
	{
		// each hook replaces a call instruction inside a game function: the function's offset plus the displacement of the call

		// PlayerCharacter::Update (virtual: the implementation has no library offset) calls UpdateAutoAimActor
		REL::Relocation<std::uintptr_t> update{ REL::Offset{ 0x53E860 } };
		_UpdateAutoAimActor = update.write_call<5, 0x5846>(Hook_UpdateAutoAimActor);

		// Projectile::AimProjectile calls ApplyAutoAimActor
		REL::Relocation<std::uintptr_t> aim{ RE::Offsets::Projectile::AimProjectile };
		_ApplyAutoAimActor = aim.write_call<5, 0x182>(Hook_ApplyAutoAimActor);

		// the three callers of CheckCriticalStrike (all call the same function): HitData::InitializeHitData (the overload with a bool),
		// HitData::InitializeHitData (the overload with a Projectile*) and HitData::InitializeExplosionData
		REL::Relocation<std::uintptr_t> initHit{ RE::Offsets::HitData::InitializeHitData_2 };
		REL::Relocation<std::uintptr_t> initProjectile{ RE::Offsets::HitData::InitializeHitData_3 };
		REL::Relocation<std::uintptr_t> initExplosion{ RE::Offsets::HitData::InitializeExplosionData };
		_CheckCriticalStrike = initHit.write_call<5, 0x4A9>(Hook_CheckCriticalStrike);
		_CheckCriticalStrike = initProjectile.write_call<5, 0xA8>(Hook_CheckCriticalStrike);
		_CheckCriticalStrike = initExplosion.write_call<5, 0x146>(Hook_CheckCriticalStrike);

		// HitData::CheckCriticalStrike calls CombatFormulas::GetCriticalChance
		REL::Relocation<std::uintptr_t> checkCrit{ RE::Offsets::HitData::CheckCriticalStrike };
		_GetCriticalChance = checkCrit.write_call<5, 0x9B>(Hook_GetCriticalChance);

		REX::INFO("real-time VATS build {} {}: installed (UpdateAutoAimActor {:#x}, ApplyAutoAimActor {:#x}); lock key {:#x}, next {:#x}, previous {:#x}",
			__DATE__, __TIME__, _UpdateAutoAimActor.address(), _ApplyAutoAimActor.address(),
			Settings::KeyLock.GetValue(), Settings::KeyNext.GetValue(), Settings::KeyPrevious.GetValue());
	}
}

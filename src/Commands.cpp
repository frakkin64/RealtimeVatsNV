#include "Commands.h"

// ------------------------------------------------------------------------------------------------- TemplateHello
// Shows argument extraction: `TemplateHello [value]` logs the optional integer and returns it (or 1).
static NVSE::ParamInfo kTemplateHelloParams[] = {
	NVSE::MakeParam("value", NVSE::ParamType::kInteger, true),
};

NVSE_DEFINE_COMMAND(TemplateHello, "example command of the commonlibnv template", false, kTemplateHelloParams);

bool Cmd_TemplateHello_Execute(NVSE_COMMAND_ARGS)
{
	*result = 1.0;

	std::int32_t value{ 0 };
	bool         hasValue{ false };
	if (const auto script = NVSE::GetScriptInterface()) {
		// NVSE's ExtractArgsEx: the parameters after the context are pointers to the extracted values
		hasValue = script->ExtractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, &value);
		if (hasValue) {
			*result = static_cast<double>(value);
		}
	}

	REX::INFO("TemplateHello called (this reference: {}, extracted: {}, value: {})", static_cast<const void*>(thisObj), hasValue, value);
	return true;
}

// ------------------------------------------------------------------------------------------------- TemplateSelfTest
// Calls into game code through the generated definitions and checks the results against what is known about the
// player reference (form id 0x14). Returns the number of failed checks (0 = everything agrees). Details go to the log.
NVSE_DEFINE_COMMAND_NOARGS(TemplateSelfTest, "checks the CommonLibNV definitions against the running game", false);

namespace
{
	int g_checks{ 0 };
	int g_failed{ 0 };

	void Check(bool a_ok, const char* a_what)
	{
		++g_checks;
		if (!a_ok) {
			++g_failed;
		}
		if (a_ok) {
			REX::INFO("SelfTest [ ok ] {}", a_what);
		} else {
			REX::ERROR("SelfTest [FAIL] {}", a_what);
		}
	}
}

bool Cmd_TemplateSelfTest_Execute(NVSE_COMMAND_ARGS)
{
	g_checks = g_failed = 0;

	// 1. a cdecl function into game code: TESForm::GetFormByNumericID (RVA 0x839C0)
	const auto form = RE::TESForm::GetFormByNumericID(0x14);
	Check(form != nullptr, "GetFormByNumericID(0x14) returns the player reference");
	if (form) {
		// 2. members of TESForm at their derived offsets
		Check(form->iFormID == 0x14, "TESForm::iFormID (0x0C) == 0x14");
		REX::INFO("SelfTest form type (0x04): 0x{:02X}, flags (0x08): 0x{:08X}", form->cFormType, form->iFormFlags);

		// 3. a virtual call (slot numbering of the x86 vtable)
		const auto savedType = static_cast<std::uint32_t>(form->GetSavedFormType());
		REX::INFO("SelfTest GetSavedFormType() -> 0x{:02X}", savedType);
		Check(savedType == form->cFormType, "virtual GetSavedFormType() matches TESForm::cFormType");

		// 4. TESObjectREFR members: base object (0x20), position (0x30), parent cell (0x40)
		const auto ref = static_cast<RE::TESObjectREFR*>(form);
		const auto base = ref->data.pObjectReference;
		Check(base != nullptr, "TESObjectREFR::data.pObjectReference (0x20) is set");
		if (base) {
			REX::INFO("SelfTest base object form id: 0x{:08X}", base->iFormID);
			Check(base->iFormID == 0x7, "the player's base object is form 0x7");
		}

		const auto& pos = ref->data.Location;
		REX::INFO("SelfTest position (0x30): {:.1f}, {:.1f}, {:.1f}", pos.x, pos.y, pos.z);
		Check(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z) && (pos.x != 0.0f || pos.y != 0.0f), "TESObjectREFR::data.Location holds a plausible position");

		// at the main menu the player reference exists but is not placed in a cell (its position is the default 2048, 2048, 128)
		const auto cell = ref->pParentCell;
		const bool unplaced = pos.x == 2048.0f && pos.y == 2048.0f && pos.z == 128.0f;
		if (!cell && unplaced) {
			REX::WARN("SelfTest [skip] the player is not in a cell yet (main menu?): load a save and run TemplateSelfTest again to check pParentCell (0x40)");
		} else {
			Check(cell != nullptr, "TESObjectREFR::pParentCell (0x40) is set");
		}
		if (cell) {
			REX::INFO("SelfTest parent cell form id: 0x{:08X}, form type 0x{:02X}", cell->iFormID, cell->cFormType);
			Check(RE::TESForm::GetFormByNumericID(cell->iFormID) == static_cast<RE::TESForm*>(cell), "the parent cell is found by its own form id");
		}
	}

	REX::INFO("SelfTest finished: {}/{} checks passed", g_checks - g_failed, g_checks);
	*result = static_cast<double>(g_failed);
	return true;
}

namespace Plugin::Commands
{
	void Register(const NVSE::LoadInterface* a_nvse)
	{
		// Every plugin that adds script commands must own a range of opcodes. Register yours with the NVSE team;
		// 0x2000 is the range NVSE reserves for testing (kNVSEOpcodeTest).
		a_nvse->SetOpcodeBase(0x2000);
		a_nvse->RegisterCommand(kCommandInfo_TemplateHello);
		a_nvse->RegisterCommand(kCommandInfo_TemplateSelfTest);
	}
}

#include "RealtimeVats.h"
#include "Settings.h"

NVSE_PLUGIN_QUERY(const NVSE::QueryInterface* a_nvse, NVSE::PluginInfo* a_info)
{
	a_info->infoVersion = NVSE::PluginInfo::kInfoVersion;
	a_info->name = NVSE_PLUGIN_NAME;
	a_info->version = NVSE_PLUGIN_VERSION;

	// the editor (GECK) is not supported
	if (a_nvse->IsEditor()) {
		return false;
	}

	// the hook sites are specific to this runtime
	if (a_nvse->PackedRuntimeVersion() != NVSE::PACKED_RUNTIME_1_4_0_525) {
		return false;
	}

	return true;
}

NVSE_PLUGIN_LOAD(const NVSE::LoadInterface* a_nvse)
{
	const auto config = REX::TOML::SettingStore::GetSingleton();
	config->Init("Data/NVSE/Plugins/RealtimeVatsNV.toml", "Data/NVSE/Plugins/RealtimeVatsNV_custom.toml");
	config->Load();

	const auto initInfo = NVSE::InitInfo{
		.logLevel = REX::ELogLevel{ Plugin::Settings::LogLevel.GetValue() },
		.pluginName = NVSE_PLUGIN_NAME,
		.pluginVersion = NVSE_PLUGIN_VERSION
	};

	NVSE::Init(a_nvse, initInfo);

	Plugin::RealtimeVats::Install();

	if (const auto messaging = NVSE::GetMessagingInterface()) {
		messaging->RegisterListener(Plugin::RealtimeVats::OnMessage);
	}

	return true;
}
